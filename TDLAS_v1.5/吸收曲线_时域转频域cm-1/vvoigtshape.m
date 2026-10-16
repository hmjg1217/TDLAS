  % This is the sub-program to simulate the lineshape function of TDLAS.
function PhiV = vvoigtshape(par,niu);
A = [-1.2150 -1.3509 -1.2150 -1.3509];    B = [ 1.2359  0.3786 -1.2359 -0.3786];
C = [-0.3085  0.5906 -0.3085  0.5906];    D = [ 0.0210 -1.1858 -0.0210  1.1858];
nniu0 = par(1);  SS = par(2);   ddeltaG = par(3);   ddeltaL = par(4); %【中心位置μ0，线型强度，高斯FWHM,洛伦兹FWHM】
nniu = niu; ddeltaair = 0;                                            %niu 频域，ddeltaair 空气中线宽偏移量修正项
        
PhiG = 2./ddeltaG*sqrt(log(2)/pi);    a = ddeltaL./ddeltaG*sqrt(log(2));
% PhiG 高斯线型的归一化因子,将高斯线型函数的积分归一化为 1。
% a 洛伦兹线宽与高斯线宽的比例,a越大越接近洛伦兹，越小越接近高斯线型。物理意义：将洛伦兹线宽ddeltaL转换为与高斯线宽ddeltaG同尺度的比例参数。

%频率偏移y  ，表示当前频率nniu与中心频率nniu0   之间的偏移量。
y = 2*sqrt(log(2))*(nniu-nniu0-ddeltaair);    PPhiV = y*0;%初始化   PPhiV   为 0
    for k = 1:4
        PPhiV = PPhiV + PhiG.*(C(k).*(a-A(k))+D(k)*(y./ddeltaG-B(k)))./((a-A(k)).^2+(y./ddeltaG-B(k)).^2);
    end
    
PhiV = PPhiV.*SS; 

%用 “多项式之比”（有理函数）模拟 Voigt 函数的形状，通过多组系数拟合优化精度。
%计算得到的线型函数PPhiV乘以线型强度SS，得到最终的线型函数值PhiV  






